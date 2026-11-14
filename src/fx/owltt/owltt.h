/*
 * Copyright © 2024-2025 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "fx/owltt/owltt_types.h"

#include <array>
#include <functional>
#include <vector>


// OWLTT — 3-band upward/downward compressor (OTT-style) with tonal shaping via CHARACTER and VIBE zones.
//
// Ported from the firmware's multiband.h. Ten crossover types, full 8-zone character and vibe
// computation, linked parameter shift, per-band M/S width, soft clipping, and linear gain smoothing.

namespace feathers::fx::owltt {

class Owltt {
public:
	Owltt();

	void prepare(double sampleRate);
	void reset();

	void setTarget(const Settings& settings);
	/// KEY TRACK'S NOTE — latched while a key is held (the Phaser's contract; see `trackedCrossoverHz`).
	void setNoteContext(const device::NoteContext& note) noexcept;
	/// THE HOST'S RESET forgets the note: no note yet, the knobs rule.
	void forgetNote() noexcept;
	/// The note's Hz while Key Track places the crossovers, 0 while it does not (reading 17).
	[[nodiscard]] double noteHzReading() const noexcept;
	[[nodiscard]] const Settings& target() const { return target_; }

	void processStereo(float* left, float* right, int frames);

	/// THE LARGEST BLOCK ONE PASS OF THE ENGINE HANDLES, and it is a HARD bound rather than a
	/// preference: `processStereo`'s band splitting uses eight `std::array<float, kSliceFrames>` on
	/// the stack and `memcpy`s `frames` samples into two of them. A call with more than this many
	/// frames wrote past them — a stack smash on a public entry point that accepts the call, with
	/// nothing between it and `FxRunner::processStereo`, which checks only `frames <= 0`.
	///
	/// UNREACHABLE THROUGH THE PLUGIN AND REACHABLE THROUGH THE API, which is why it is fixed rather
	/// than documented: `Synth::render` cuts every chunk at `min(n - done, kRenderChunk = 32, ...)`,
	/// so nothing in the shipping path has ever passed more than 32. Anything holding this device
	/// directly could, and the failure mode is not a wrong number.
	static constexpr int kSliceFrames = 512;

	/// **HOW LONG THE OUTPUT AND PER-BAND GAINS TAKE TO REACH A NEW TARGET, IN SECONDS.**
	///
	/// A TIME CONSTANT RATHER THAN "ONE CALL", which is what it used to be: the gains were ramped
	/// linearly to their target across exactly `frames` samples, so the smoothing time WAS the host's
	/// chunk length. `Synth::render` cuts at `min(n - done, kRenderChunk = 32, ...)` and a CLAP host
	/// cuts the block at every event, so the chunk is short wherever a note falls — and MEASURED
	/// through the plugin, one four-event script at host blocks 256 / 64 / 100 / 441 differed by
	/// **0.0619171 worst against a 4.75021 peak**, where the other four devices are bit-identical. A
	/// sound parameter nobody can see or set.
	///
	/// **1.78 ms, CHOSEN BY MEASURING AGAINST TODAY'S BEHAVIOUR AT A 256-SAMPLE BLOCK — and no
	/// constant matches it exactly, which is worth saying rather than papering over.** A linear ramp
	/// ARRIVES at its target; a one-pole APPROACHES one. The two have different shapes, so matching
	/// the initial slope and matching the landing point give different answers, and the measurement
	/// says so plainly. One 256-sample call through the ops road, against the old ramp's
	/// `sumAbs 39.4628 / peak 0.389283` at enabled defaults and `155.065 / 2.2796` at a heavily
	/// compressing setting:
	///
	///     tau        defaults sumAbs   defaults peak    heavy sumAbs    heavy peak
	///     256/1      +0.9%             +0.02%           -24%            -41%
	///     256/2      -0.4%             -0.35%           +8.8%           -16%
	///     256/3      -1.2%             -0.7%            +29%            -4.8%     <- chosen
	///     256/4      -1.6%             -1.0%            +42%            -0.5%
	///     256/6      -2.1%             -1.6%            +57%            +5.0%
	///
	/// At gentle settings every candidate is within 2%, so the choice is made where it matters. 256/3
	/// is the classic "settles inside the block" constant — a one-pole closes 95% of its distance in
	/// three time constants, which is what the ramp did in one block — and it is the row that sits
	/// between the two metrics rather than at an extreme of either.
	///
	/// **A STARTING POINT, NOT A DECISION**, which is exactly what the backlog asked for: the table is
	/// here so it can be moved by ear from somewhere stated. Larger is slower and smoother; smaller
	/// tracks harder and is louder under heavy compression.
	static constexpr double kGainSmoothingSeconds = 256.0 / 3.0 / 48000.0;

	/// **THE BLOCK LENGTH TODAY'S BEHAVIOUR IS DEFINED AT.** The two block-dependent smoothers below
	/// are both re-expressed so that a call of exactly this many frames reproduces what the device did
	/// before, and any other length now gives the same answer per SAMPLE instead of per call. One
	/// constant rather than two so the two halves cannot drift into different references.
	static constexpr int kReferenceFrames = 256;

	/// **BELOW THIS THE TAIL IS OVER AND THE STATE IS CLEARED**, and **HOW LONG THE INPUT MUST HAVE
	/// BEEN EXACTLY SILENT** before that is allowed. Both are `MultimodeFilter`'s, restated rather
	/// than shared because the two devices have no header in common and a `using` from one effect
	/// into another would be a coupling neither wants; the reasoning is written out there in full.
	///
	/// IN SHORT: a geometric decay does not reach zero — it walks into the denormals and parks, which
	/// `puck_motion.h` already paid for and fixed with `kRestThreshold`. MEASURED here, after a 0.8
	/// burst OWLTT produced a non-zero sample in **400 blocks of 400** at 9.5008e-43, *identical to
	/// every digit across 2,800 blocks*.
	///
	/// AND THE CONDITION IS ABOUT THE INPUT, not the output: a quiet output is not a quiet state. That
	/// cost two of `test_fx_filter`'s impulse-response cases 0.472 dB and 1.24 dB when the first
	/// version of this flushed on the output alone.
	static constexpr float kRestLevel = 1.0e-30f;
	static constexpr double kRestSilentSeconds = 1.0;

	/// TEST ONLY — the attack and release the band computed from the knob, in ms. The claim under
	/// test is "a 0.5 ms attack takes 0.5 ms", and a render cannot be asked what a coefficient is.
	[[nodiscard]] float attackMsForTest(size_t band) const { return bands_[band].getAttackMS(); }
	[[nodiscard]] float releaseMsForTest(size_t band) const { return bands_[band].getReleaseMS(); }
	/// TEST ONLY — the per-direction ratios a band's compressor was handed (knob units), and the input gain it
	/// scales its band by: the target the slice was heading for and where the smoother has got to.
	[[nodiscard]] float ratioUpForTest(size_t band) const { return bands_[band].getRatioUp(); }
	[[nodiscard]] float ratioDownForTest(size_t band) const { return bands_[band].getRatioDown(); }
	[[nodiscard]] float inputGainTargetForTest(size_t band) const noexcept { return inputGainTarget_[band]; }
	[[nodiscard]] float inputGainForTest(size_t band) const noexcept { return inputGain_[band]; }

	/// Per-band gain reduction in dB (for metering / OTT display). Test-only accessor.
	[[nodiscard]] float getBandGainReductionDB(size_t band) const {
		return bands_[band].getGainReductionDB();
	}

	/// **ONE CHANNEL OF ONE BAND (backlog LQ)** — 0 is L (or M), 1 is R (or S). The gain reduction its envelope
	/// is applying, in nepers, and the detector level its gain computer read, after Link's blend. Test-only:
	/// "Link 0 leaves R untouched" is a claim about these numbers, which a render that also carries the
	/// crossovers, the clippers and the other band cannot give back.
	[[nodiscard]] float channelGainReductionForTest(size_t band, size_t channel) const;
	[[nodiscard]] float channelInputLevelLogForTest(size_t band, size_t channel) const;

	/// Per-band input level in log domain (for metering). Test-only accessor.
	[[nodiscard]] float getBandInputLevelLog(size_t band) const {
		return bands_[band].getInputLevelLog();
	}

	/// THE TWO THRESHOLDS A BAND IS ACTUALLY COMPARING AGAINST, in knob units. Test-only.
	///
	/// ADDED TO FIND THE INVERSION. Every diagnosis of "the loud band does not compress" so far has
	/// been reasoning about `calculateGain` from the outside; these let a probe print the comparison
	/// itself rather than infer it. Same argument as `voiceFilterTargetForTest` elsewhere in this
	/// repo: if the claim is "this value is X at this instant", read the value.
	[[nodiscard]] float getBandThresholdDown(size_t band) const {
		return bands_[band].getThresholdDown();
	}
	[[nodiscard]] float getBandThresholdUp(size_t band) const {
		return bands_[band].getThresholdUp();
	}

	[[nodiscard]] float getBandRatioDown(size_t band) const { return bands_[band].getRatioDown(); }
	[[nodiscard]] float getBandOutputLevelLinear(size_t band) const {
		return bands_[band].getOutputLevelLinear();
	}

	/// SET THE METER BRIDGE — the DSP publishes per-band GR and level to this bridge, which owns the
	/// atomics and outlives every editor open/close cycle. Called by the lane after the DSP is
	/// constructed. Non-owning; the bridge outlives the DSP.
	///
	/// @param callback Called every buffer with:
	///   - slot: which OWLTT instance
	///   - grDB: per-band gain reduction, normalized to ±1.0 (negative = downward compression)
	///   - inputLevelDB: per-band input level, normalized to 0..1 (dBFS-linear scale)
	///   - outputLevelDB: per-band output level, normalized to 0..1 (dBFS-linear scale)
	///   - clipping: true if output is clipping (with hold timer)
	/// THE METER READINGS THE FIRST PORT LEFT OUT (backlog EL): a light per band while its clipper is
	/// working, and the mean |gain change| across the bands in dB. Carried beside the five original
	/// arguments rather than folded into them, so every road that publishes meters — the lane's bridge,
	/// the rack's publisher, the generic readings sink — receives them by construction.
	struct MeterExtras {
		std::array<bool, 3> bandSaturating { false, false, false };
		float combinedGrDb = 0.0f;
		/// BACKLOG LQ: each band's SECOND channel's gain reduction (R, or S in M/S), scaled as `grDB` is — which
		/// now carries the FIRST channel's. At Link 100% the two are the same number.
		std::array<float, 3> secondChannelGr { 0.0f, 0.0f, 0.0f };
		/// KEY TRACK: the note's Hz while it places the crossovers, 0 while it does not (reading 17).
		float noteHz = 0.0f;
	};
	using MeterCallback = std::function<void(size_t slot, const std::array<float, 3>& grDB,
	                                        const std::array<float, 3>& inputLevelDB,
	                                        const std::array<float, 3>& outputLevelDB, bool clipping,
	                                        const MeterExtras& extras)>;
	void setMeterCallback(MeterCallback callback);

	/// SET THE SLOT. Called by the host after construction.
	///
	/// **NOTHING CONSUMES THIS NUMBER TODAY — the meters do NOT key on it.** This comment used to say
	/// "so per-slot state (like meters) can be keyed by slot", and that coupling does not exist:
	/// `slot_` is written here, read in exactly one place (passed to `meterCallback_`), and the
	/// consumer in `feathers_clap.cpp` DISCARDS it — its first parameter is unnamed, and it publishes
	/// against the slot captured from the DSP connector instead. Measured, not read off the grep:
	/// replacing this assignment with `slot_ = 12345` leaves all 32 suites green, meter coverage
	/// included.
	///
	/// So it is write-only in effect, and the number handed to it may be changed freely. That mattered
	/// because `FxRunner` passed a FLATTENED `instance * kMaxFxSlots + slot` index here rather than a
	/// slot; **it passes the slot now** (2026-08-26), and this note is what established that the
	/// correction had nothing downstream to break.
	///
	/// A MEASUREMENT IS NOT A GUARANTEE, THOUGH, and "write-only" is exactly the property that decays
	/// silently. `tests/test_lane_rack.cpp` asserts the number a device is actually told — its LANE,
	/// now that the lane runs on the rack engine, kept across a reorder — so the next reader of this
	/// number will find out at a red test rather than at a wrong meter.
	void setSlot(size_t slot);

	/// **THE GAIN SMOOTHER'S COEFFICIENT, for tests only** — the five-line seam CLAUDE.md prescribes,
	/// and it exists because the claim is about a NUMBER that no render can be asked for. "This device
	/// smooths at a time constant rather than at the host's block length" is a statement about
	/// `gainSmoothAlpha_`, and a comparison of two renders would pass whenever anything differed —
	/// which, while the peak DETECTOR is still block-windowed, it always does.
	[[nodiscard]] float gainSmoothAlphaForTest() const noexcept { return gainSmoothAlpha_; }
	/// TWO VALUE SEAMS FOR THE UNBOUNDED COORDINATES (backlog EH, shape 2): the vibe knee phase, which
	/// reads the lap's fraction through a phi triangle, and the character response, which reads the
	/// feel phases. A rendered buffer cannot say what either of them was.
	[[nodiscard]] float vibePhaseKneeForTest() const noexcept { return vibePhaseKnee_; }
	[[nodiscard]] float feelPhase033ForTest() const noexcept { return feelPhase033_; }

	/// **WHAT A BAND'S GAIN WAS LAST HEADING FOR**, for tests only — the other half of the pair below,
	/// and the one that makes the claim assertable rather than merely observable. "The gain approaches
	/// its target over a time constant" needs both numbers: with only the gain, a snap back to the
	/// target and a one-pole that has nearly arrived are the same reading, and the first draft of the
	/// case could not tell them apart.
	[[nodiscard]] float lastTargetBandGainForTest(size_t band) const noexcept {
		return band < lastTargetBandGain_.size() ? lastTargetBandGain_[band][0] : 0.0f;
	}

	/// **WHERE A BAND'S GAIN HAS GOT TO**, for tests only. The companion to the above and the only
	/// thing that can see the end-of-call snap this fix removed: with the snap, the gain lands exactly
	/// on its target at every call boundary however the smoother is written, so the time constant is
	/// still the block and only the shape of the approach has changed.
	/// TEST ONLY — the meter peaks, whose DECAY was a per-call rate until 2026-08-28. The claim is
	/// "the meter falls at a rate in milliseconds", which no render can be asked and which a
	/// comparison of two renders cannot distinguish from "the meter fell".
	[[nodiscard]] float bandOutputPeakForTest(size_t band) const {
		return bands_[band].getBandOutputPeak();
	}
	[[nodiscard]] float outputPeakForTest() const noexcept { return outputPeak_; }
	/// The meter extras of backlog EL, as the readings pack them: whether band `b`'s light is on, and
	/// the combined gain reduction in dB.
	[[nodiscard]] bool bandSaturatingForTest(size_t band) const noexcept { return bandSaturating_[band]; }
	[[nodiscard]] float combinedGainReductionDbForTest() const noexcept { return combinedGainReductionDb_; }
	/// How many readings this device publishes: 3 GR, 3 input, 3 output, clip, 3 saturating, combined GR, and
	/// since backlog LQ the second channel's GR per band (14..16; 0..2 are the first channel's).
	static constexpr size_t kReadingCount = 18;
	//-- OWLTT parity sweep seams (2026-09-16); each a value the audio cannot give back. ----------//
	/// A band's stereo width after the zone table, vibe and feel have written it.
	[[nodiscard]] float bandWidthForTest(size_t band) const noexcept { return bandWidth_[band]; }
	/// BACKLOG LR: the width the band actually SENDS ON — the zone's above, scaled by Width and the band's own Width.
	[[nodiscard]] float effectiveBandWidthForTest(size_t band) const noexcept { return effectiveBandWidth_[band]; }
	/// The twist the vibe pass decided, and the twist the Twisted splitter is actually running —
	/// equal once the crossovers are updated AFTER the vibe pass (they lagged a slice before).
	[[nodiscard]] float vibeTwistForTest() const noexcept { return vibeTwist_; }
	[[nodiscard]] float twistedCrossoverTwistForTest() const { return twisted_[0].getTwist(); }
	/// The high corner the splitters were given, after the 100 Hz gap above the low one.
	[[nodiscard]] float effectiveHighCrossoverHzForTest() const noexcept { return effectiveHighCrossoverHz_; }
	/// The low corner the splitters were last handed, Hz (the knob, or the tracked corner).
	[[nodiscard]] float effectiveLowCrossoverHzForTest() const noexcept { return effectiveLowCrossoverHz_; }
	/// The vibe depth the zone tables were modulated by — 0.55 x the coordinate's lap fraction.
	[[nodiscard]] float vibeModAmountForTest() const noexcept { return vibeModAmount_; }

	[[nodiscard]] float smoothedGainForTest(size_t band) const noexcept {
		return band < smoothedGain_.size() ? smoothedGain_[band][0] : 0.0f;
	}
	/// ONE CHANNEL'S smoothed gain (backlog LQ): 0 is L (or M), 1 is R (or S). The one above is channel 0.
	[[nodiscard]] float smoothedGainForTest(size_t band, size_t channel) const noexcept {
		return band < smoothedGain_.size() && channel < 2 ? smoothedGain_[band][channel] : 0.0f;
	}

	/// WHAT IT WAS TOLD, for tests only.
	///
	/// The five-line seam CLAUDE.md prescribes: the claim is "this device believes it is in slot 2 at
	/// this instant", and the meter callback — the only other road to this number — cannot answer it
	/// on a device the runner is not currently running. It was written when `FxRunner` stopped handing
	/// devices a FLATTENED pool index and started handing them their slot, which is a change no render
	/// can express and only a value can.
	///
	/// AND IT IS WHAT GUARDS THE NOTE ABOVE `setSlot`. That note is a measurement — nothing consumes
	/// this number today — and a measurement about absence is the kind that stops being true without
	/// anybody editing the line it describes.
	[[nodiscard]] size_t slotForTest() const noexcept { return slot_; }

	/// **BLEND, PER SAMPLE, FOR THE NEXT CALL** — the host's tick window over exactly that call's frames, or null while
	/// Blend is not moving (`AudioOps::setFieldWindows`, backlog FK). A routed Blend stepped once per tick before.
	void setBlendRun(const float* run) noexcept { blendRun_ = run; }
	/// **DRIVE, PER SAMPLE, FOR THE NEXT CALL** (backlog NB) — the same window, for the gain in front of the dynamics.
	void setDriveRun(const float* run) noexcept { driveRun_ = run; }

	/// **THE LOOKAHEAD'S DELAY LINE EMPTIED (backlog ND)** — `prepare` and the host's `reset` call it. The device's own
	/// resets (a bypass, the rest flush) do NOT: the line is the latency the host was told, and emptying it would drop up
	/// to 5 ms of audio that is still on its way out.
	void clearLookahead();
	/// HOW MANY SAMPLES THE LAST CALL DELAYED THE AUDIO BY — the delay as applied, for a case to set beside the latency.
	[[nodiscard]] uint32_t lookaheadAppliedForTest() const noexcept { return lookLen_; }

private:
	/// **THE SHARED DELAY, ONE SAMPLE OF IT (backlog ND)**: every band's two channels and the dry's two go in, and what
	/// went in `lookLen_` samples ago comes out, in place. One position for all eight, so the bands cannot drift apart.
	/// A no-op at Off, which is what keeps Off the device before ND to the bit.
	void lookaheadStep(std::array<float*, 8> lanes, int frames);
	/// **THE COORDINATES THE DSP READS, THROUGH THE MODE (backlog IW)** — `zoneModeCoordinate`: the value itself in Free,
	/// `lap + (mode + amount) / 8` in a mode. Every read of Character and Vibe goes through these.
	[[nodiscard]] float characterCoordinate() const { return zoneModeCoordinate(target_.characterMode, target_.character); }
	[[nodiscard]] float vibeCoordinate() const { return zoneModeCoordinate(target_.vibeMode, target_.vibe); }
	/// ONE PASS OF THE ENGINE OVER AT MOST `kSliceFrames` FRAMES. `processStereo` is the loop that
	/// feeds it; everything the device does lives here.
	void processStereoSlice(float* left, float* right, int frames);

	void updateCrossoverFrequencies();
	/// EVERY CROSSOVER'S STATE CLEARED — on a reset, and on a crossover TYPE change (backlog EH, shape
	/// 6): the type that comes in would otherwise resume from whatever it held when it last ran.
	void resetCrossovers();
	void computeCharacterAndVibe();
	void updateMeters(int frames);

	static constexpr size_t kNumBands = 3;

	// Crossover instances, indexed by [channel] (0 = L, 1 = R). The render loop selects the active
	// type via switch on crossoverType_ and processes each channel through its own instance, matching
	// the firmware's parallel L/R stereo processing.
	std::array<APCrossover1, 2> ap1_;         // Type 0 — AP 6dB
	std::array<APCrossover2, 2> ap2_;         // Type 1 — Quirky
	std::array<AllpassCrossoverTwisted, 2> twisted_; // Type 2 — Twisted
	std::array<APCrossover3, 2> ap3_;         // Type 3 — Weird
	std::array<LR2CrossoverFast, 2> lr2fast_; // Type 4 — LR2 Fast
	std::array<LR2CrossoverFull, 2> lr2full_; // Type 5 — LR2 Full
	std::array<LR4CrossoverFast, 2> lr4fast_; // Type 6 — LR4 Fast
	std::array<LR4CrossoverFull, 2> lr4full_; // Type 7 — LR4 Full
	std::array<AllpassCrossoverTwist3, 2> twist3_; // Type 9 — Twist3
	// Type 8 (Inverted) reuses ap1_ with swapped low/high bands in the render loop.

	std::array<BandCompressor, kNumBands> bands_; // the constant the literal 3 was standing in for (backlog EH, shape 1)
	/// **EACH BAND'S TWO CHANNELS (backlog LQ)** — [band][0] is L (or M), [band][1] is R (or S). Each hears only its
	/// own channel; `bands_` above stays the band's LINKED detector (the louder channel, as before LQ), and Link
	/// blends each channel's level toward it before the channel's own gain computer runs. These are what the
	/// audio is gained by; at Link 100% each runs exactly `bands_`'s trajectory.
	std::array<std::array<BandCompressor, 2>, kNumBands> channels_;
	Settings target_ {};
	/// See `setBlendRun`; consumed by the next `processStereo`, which reads it at `blendRunAt_`.
	const float* blendRun_ = nullptr;
	const float* driveRun_ = nullptr; ///< `setDriveRun`'s, read at the same offset as the Blend run
	int blendRunAt_ = 0;
	/// The crossover type the last slice ran — 255 until one has — so a change is a change.
	uint8_t lastCrossoverType_ = 255;
	/// Whether the last block ran — so the first bypassed block resets, once (backlog EH, shape 6).
	bool running_ = false;
	/// One of the six feel phases, kept for `feelPhase033ForTest` — the lap count times a phi
	/// constant, wrapped; the number a rendered buffer cannot give back.
	float feelPhase033_ = 0.0f;
	double sampleRate_ = 48000.0;
	/// The one-pole coefficient `kGainSmoothingSeconds` becomes at this sample rate. Recomputed in
	/// `prepare` and nowhere else, so nothing in the render path can make it a function of the block.
	float gainSmoothAlpha_ = 1.0f;
	/// Consecutive samples of exactly silent INPUT, and the wait in samples at the prepared rate.
	/// See `kRestSilentSeconds`.
	int restSilence_ = 0;
	int restSilenceNeeded_ = 48000;
	size_t slot_ = 0;
	uint32_t frameCount_ = 0;

	// Character-derived parameters (computed in computeCharacterAndVibe).
	float response_ = 0.5f;
	std::array<float, 3> bandWidth_ {0.7f, 0.85f, 0.9f};
	/// BACKLOG LR: `bandWidth_` scaled by the Width controls (`effectiveBandWidthOf`) — what the render loop uses.
	std::array<float, 3> effectiveBandWidth_ {0.7f, 0.85f, 0.9f};
	float knee_ = 0.2f;
	std::array<float, 3> timingOffset_ {0.0f, 0.0f, 0.0f};
	std::array<float, 3> skewOffset_ {0.0f, 0.0f, 0.0f};
	float alpha_ = 0.31f;
	float oneMinusAlpha_ = 0.69f;
	float upDownSkew_ = 0.0f;
	bool characterComputed_ = false;
	float characterKnob_ = 0.0f;

	// Vibe-derived phase offsets (computed in computeCharacterAndVibe).
	float vibeTwist_ = 1.0f;
	/// The high corner after the gap clamp — what `updateCrossoverFrequencies` handed the splitters.
	float effectiveHighCrossoverHz_ = 0.0f;
	float effectiveLowCrossoverHz_ = 0.0f;
	// KEY TRACK: the latched note, and the corners' glide (log2 Hz).
	bool heardNote_ = false;
	double noteSemitones_ = 60.0;
	double keyLog2Now_[2] {};
	bool keyPrimed_ = false;
	int sliceFrames_ = 1;
	/// The vibe modulation depth the last non-OWLTT-zone pass applied: 0.55 x the lap's fraction.
	float vibeModAmount_ = 0.0f;
	std::array<float, 3> vibePhaseWidth_ {0.0f, 0.0f, 0.0f};
	float vibePhaseKnee_ = 0.0f;
	std::array<float, 3> vibePhaseTiming_ {0.0f, 0.0f, 0.0f};
	std::array<float, 3> vibePhaseSkew_ {0.0f, 0.0f, 0.0f};
	float vibePhaseOffset_ = 0.0f;
	float feelPhaseOffset_ = 0.0f;

	// Gain smoothing state — linear ramp from current to target gain across the buffer.
	/// PER BAND, PER CHANNEL (backlog LQ): [band][0] is L (or M), [band][1] is R (or S).
	std::array<std::array<float, 2>, 3> smoothedGain_ {{{1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}}};
	/// The target the last slice was heading for. Written once per slice and read only by
	/// `lastTargetBandGainForTest`; the sample loop never touches it.
	std::array<std::array<float, 2>, 3> lastTargetBandGain_ {{{1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}}};
	float smoothedOutputGain_ = 1.0f;
	/// EACH BAND'S INPUT LEVEL AS A GAIN: where the per-sample smoother has got to, and its target. 1 is 0 dB.
	std::array<float, 3> inputGain_ { 1.0f, 1.0f, 1.0f };
	std::array<float, 3> inputGainTarget_ { 1.0f, 1.0f, 1.0f };

	// Output peak with decay (firmware: outputPeak_ = max(peakThisBuffer, outputPeak_ * 0.95)).
	float outputPeak_ = 0.0f;
	float outputPeakThisBuffer_ = 0.0f;

	// Clipping detection with hold timer (firmware: clippingHoldCounter_, kIndicatorHoldBuffers=170).
	//
	// THE FIRMWARE COUNTED BUFFERS BECAUSE IT HAD ONE BUFFER SIZE, decided at design time. A plugin
	// is handed whatever the host feels like, so a hold counted in CALLS is a hold whose length is
	// the host's buffer setting. MEASURED before the fix: the indicator held 15,072 samples (314 ms)
	// at a 32-sample block and 77,312 (1.61 s) at 512 -- 5.1x, for the same signal.
	//
	// THE REFRESH IS THE ONLY THING THAT NEEDED CONVERTING. The hold is counted in REFRESH PERIODS
	// (`kIndicatorHoldBuffers / kMeterRefreshBuffers`), so a refresh measured in samples carries the
	// hold with it and the two constants keep their firmware-facing names and ratio.
	//
	// 256 FRAMES IS THIS DEVICE'S REFERENCE BLOCK, not a number chosen here: `kGainSmoothingSeconds`
	// is `256.0 / 3.0 / 48000.0`, the master peak decays by `pow(0.95, frames / 256.0)`, and
	// band_compressor.h names it `kDecayReferenceFrames`. So at a 256-sample block this behaves
	// exactly as it did, and the device's FOUR ported per-block rates now agree about what "today"
	// means -- `processStereo` claimed three, and walked past this one.
	bool clipping_ = false;
	int clippingHoldCounter_ = 0;
	/// PER-BAND SATURATION, the firmware's `bandSaturating_[b]` (backlog EL): a band whose post-clip
	/// peak passed 0 dBFS — half its +6 dB knee, so the clipper is already working — holds its light
	/// for the same ~500 ms the output clip light holds. Refreshed on the meter grid with it.
	std::array<bool, 3> bandSaturating_ { false, false, false };
	std::array<int, 3> bandSaturationHold_ { 0, 0, 0 };
	static constexpr float kBandSaturationThreshold = 1.0f;
	/// THE COMBINED GAIN REDUCTION, in dB: the mean |gain change| across the three bands — the
	/// firmware's `getGainReduction()`, which scaled it to 0..127 for its display; here it is dB.
	float combinedGainReductionDb_ = 0.0f;
	static constexpr int kIndicatorHoldBuffers = 170;
	static constexpr int kMeterRefreshBuffers = 35;
	static constexpr double kMeterReferenceFrames = 256.0;
	/// The refresh period in SAMPLES, from the buffer count above at the reference block. Set in
	/// `prepare`, so it follows the host's sample rate as well as its block size.
	int meterRefreshPeriodSamples_ = static_cast<int>(kMeterRefreshBuffers * kMeterReferenceFrames);
	int meterRefreshSamples_ = 0;

	// Live meter bridge — the DSP publishes per-band GR and level to this bridge, which owns the
	// atomics and outlives every editor open/close cycle. Non-owning; the bridge outlives the DSP.
	MeterCallback meterCallback_;

	/// BACKLOG ND'S LINE: `kLookaheadLanes` floats per sample, interleaved, sized at `prepare` for the longest choice at
	/// the prepared rate (10 ms is 1920 samples at 192 kHz), so the audio thread never allocates.
	static constexpr size_t kLookaheadLanes = 8;
	std::vector<float> look_;
	uint32_t lookCap_ = 0;
	uint32_t lookPos_ = 0;
	/// The delay now in force, from `lookaheadSamplesFor(target_.lookahead, rate)`; a change takes effect at once.
	uint32_t lookLen_ = 0;
};

/// HOW THE HOST RUNS THIS DEVICE'S AUDIO — defined in owltt.cpp.
extern const device::AudioOps kAudioOps;

} // namespace feathers::fx::owltt
